#include "l0/parquet.hpp"
#include "l0/query.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

using namespace l0;
namespace {
/// @brief Экранирует строку для JSON и заключает её в двойные кавычки.
std::string json(std::string_view s) {
    static const char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            // Управляющие байты нельзя помещать в JSON-строку без экранирования.
            if (c < 32) {
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else
                out += char(c);
        }
    }
    return out + '"';
}
/// @brief Разбирает десятичный u64, отклоняя знак, посторонние символы и переполнение.
uint64_t number(const std::string& s) {
    require(!s.empty() && s.size() <= 20, "invalid integer: " + s);
    uint64_t n = 0;
    for (char c : s) {
        require(c >= '0' && c <= '9' && n <= (UINT64_MAX - unsigned(c - '0')) / 10,
                "invalid integer: " + s);
        // Проверка перед умножением предотвращает переполнение самого вычисления.
        n = n * 10 + unsigned(c - '0');
    }
    return n;
}
struct Args {
    std::map<std::string, std::string> values;
    /// @brief Собирает CLI-опции, распознавая флаги без значений и отклоняя дубликаты.
    Args(int argc, char** argv, int start) {
        for (int i = start; i < argc; ++i) {
            std::string key = argv[i];
            require(key.rfind("--", 0) == 0, "expected --option: " + key);
            require(!values.count(key), "duplicate option: " + key);
            if (key == "--json" || key == "--count")
                values[key] = "1";
            else {
                require(i + 1 < argc, "missing value for " + key);
                values[key] = argv[++i];
            }
        }
    }
    /// @brief Извлекает и удаляет обработанную опцию; при отсутствии возвращает fallback.
    std::string take(const std::string& key, std::string fallback = {}) {
        auto it = values.find(key);
        if (it == values.end())
            return fallback;
        auto v = it->second;
        values.erase(it);
        return v;
    }
    /// @brief Извлекает целочисленную опцию с проверкой диапазона либо возвращает fallback.
    uint64_t integer(const std::string& key, uint64_t fallback) {
        return number(take(key, std::to_string(fallback)));
    }
    /// @brief Проверяет, что после разбора не осталось неизвестных опций.
    void done() {
        require(values.empty(),
                "unknown option: " + (values.empty() ? std::string() : values.begin()->first));
    }
};
/// @brief Выводит docID и метаданные как один JSON-объект без завершающего перевода строки.
void print_document(uint32_t id, const DocumentInfo& d) {
    std::cout << "{\"doc_id\":" << id << ",\"id\":" << json(d.id) << ",\"url\":" << json(d.url)
              << ",\"source\":" << json(d.source) << ",\"row\":" << d.row
              << ",\"score\":" << std::setprecision(17) << d.score << "}";
}
/// @brief Выполняет запрос и печатает результаты с измерением времени поиска.
/// limit ограничивает выдачу; count включает полный подсчёт, as_json выбирает JSON-формат.
void run_query(const Index& index, const std::string& text, size_t limit, bool count,
               bool as_json) {
    // Измеряем разбор и поиск; открытие индекса и печать результатов сюда не входят.
    auto start = std::chrono::steady_clock::now();
    auto query = parse_query(text);
    auto result = search(index, *query, limit, count);
    double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (as_json) {
        std::cout << "{\"query\":" << json(text) << ",\"matches\":" << result.matches
                  << ",\"complete\":" << (result.complete ? "true" : "false")
                  << ",\"elapsed_ms\":" << ms
                  << ",\"blocks_decoded\":" << result.stats.blocks_decoded
                  << ",\"position_blocks_decoded\":" << result.stats.position_blocks_decoded
                  << ",\"skip_jumps\":" << result.stats.skip_jumps << ",\"hits\":[";
        bool first = true;
        for (auto id : result.hits) {
            if (!first)
                std::cout << ',';
            first = false;
            print_document(id, index.document(id));
        }
        std::cout << "]}\n";
    } else {
        for (auto id : result.hits) {
            auto d = index.document(id);
            std::cout << id << '\t' << std::setprecision(17) << d.score << '\t' << d.id << '\t'
                      << d.url << '\n';
        }
        std::cout << "matches=" << (result.complete ? "" : ">=") << result.matches
                  << " elapsed_ms=" << ms << " blocks=" << result.stats.blocks_decoded
                  << " position_blocks=" << result.stats.position_blocks_decoded
                  << " skip_jumps=" << result.stats.skip_jumps << '\n';
    }
}
/// @brief Выводит команды CLI, значения по умолчанию и правила языка запросов.
void help() {
    std::cout << R"(L0 positional Boolean search engine (C++17)
  l0 build --input fineweb-edu --output indexes/full [--memory-mb 256]
           [--max-docs 0] [--batch-size 256] [--sort-memory-mb 256]
  l0 search --index indexes/full --query 'science AND NOT fiction'
            [--limit 20] [--count] [--json]
  l0 repl --index indexes/full [--limit 20] [--count] [--json]
  l0 stats --index indexes/full
  l0 document --index indexes/full --doc-id 0

Operators (uppercase): AND, OR, AND NOT, NEAR[/n], ADJ[/n], parentheses.
Precedence: phrases > NEAR/ADJ > AND/AND NOT > OR; left associative.
NEAR defaults to 5, unordered. ADJ defaults to 1, ordered.
Distance is the difference of nearest non-overlapping token coordinates.
Double quotes form an exact phrase. --count scans all matches; otherwise
search stops at --limit and reports a lower bound. --max-docs 0 means all.
Documents are sorted by descending score BEFORE indexing; ties keep source order.
--max-docs selects the first N source rows, then ranks that subset.
English terms use Snowball (Porter2) stemming in documents and queries.
)";
}
} // namespace
/// @brief Обрабатывает команду приложения; возвращает 0 при успехе и 1 при ошибке.
/// В REPL ошибка отдельного запроса выводится без завершения сеанса.
int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") {
            help();
            return 0;
        }
        std::string command = argv[1];
        Args args(argc, argv, 2);
        if (command == "build") {
            auto input = args.take("--input", "fineweb-edu"), output = args.take("--output");
            require(!output.empty(), "--output is required");
            uint64_t mb = args.integer("--memory-mb", 256), max = args.integer("--max-docs", 0),
                     batch = args.integer("--batch-size", 256),
                     sort_mb = args.integer("--sort-memory-mb", mb);
            args.done();
            require(mb > 0 && mb <= UINT64_MAX / (1024 * 1024) && sort_mb > 0 &&
                        sort_mb <= UINT64_MAX / (1024 * 1024) && batch > 0 && batch <= INT32_MAX,
                    "invalid memory/batch size");
            // Проверяем наличие входных файлов до создания выходного каталога.
            corpus_files(input);
            auto start = std::chrono::steady_clock::now();
            Builder builder(output, mb * 1024 * 1024);
            ingest_parquet(builder, input, fs::path(output) / ".ranking", max, int64_t(batch),
                           sort_mb * 1024 * 1024);
            builder.finish();
            std::cout
                << "index=" << output << " documents=" << builder.document_count()
                << " tokens=" << builder.token_count() << " elapsed_s="
                << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                << '\n';
            return 0;
        }
        require(command == "search" || command == "repl" || command == "stats" ||
                    command == "document",
                "unknown command: " + command);
        auto root = args.take("--index");
        require(!root.empty(), "--index is required");
        Index index(root);
        if (command == "stats") {
            args.done();
            uint64_t bytes = 0, vocabulary = 0;
            for (auto& f : fs::directory_iterator(root))
                if (f.is_regular_file())
                    bytes += f.file_size();
            for (auto& s : index.segments)
                vocabulary += s->vocabulary_size();
            std::cout << "{\"documents\":" << index.documents << ",\"tokens\":" << index.token_count
                      << ",\"segments\":" << index.segments.size()
                      << ",\"segment_term_entries\":" << vocabulary << ",\"bytes\":" << bytes
                      << ",\"block_size\":" << BLOCK << ",\"ranking\":\"score-desc-stable-v1\"}\n";
            return 0;
        }
        if (command == "document") {
            uint64_t id = args.integer("--doc-id", END);
            args.done();
            require(id < index.documents, "--doc-id out of range");
            print_document(uint32_t(id), index.document(uint32_t(id)));
            std::cout << '\n';
            return 0;
        }
        size_t limit = args.integer("--limit", 20);
        bool count = !args.take("--count").empty(), as_json = !args.take("--json").empty();
        if (command == "search") {
            auto query = args.take("--query");
            args.done();
            run_query(index, query, limit, count, as_json);
        } else {
            args.done();
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line.empty())
                    continue;
                try {
                    run_query(index, line, limit, count, as_json);
                } catch (const std::exception& e) {
                    // Ошибка одной строки REPL не должна прерывать последующие запросы.
                    std::cerr << "query error: " << e.what() << '\n';
                }
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
