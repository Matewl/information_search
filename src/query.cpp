#include "l0/query.hpp"
#include "l0/tokenizer.hpp"
#include <cctype>

namespace l0 {
namespace {
struct Lexeme {
    enum Type { Word, Phrase, Left, Right, End } type;
    std::string value;
};
class Parser {
    std::string_view input_;
    size_t pos_ = 0;
    Lexeme cur_;
    size_t depth_ = 0, nodes_ = 0;
    /// @brief Читает следующую лексему, распознавая скобки, слова и экранированные фразы.
    void next() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_])))
            ++pos_;
        if (pos_ == input_.size()) {
            cur_ = {Lexeme::End, {}};
            return;
        }
        char c = input_[pos_++];
        if (c == '(') {
            cur_ = {Lexeme::Left, "("};
            return;
        }
        if (c == ')') {
            cur_ = {Lexeme::Right, ")"};
            return;
        }
        // Внутри фразы обратная косая черта экранирует следующий символ,
        // поэтому кавычка после неё не закрывает строку.
        if (c == '"') {
            std::string s;
            bool closed = false;
            while (pos_ < input_.size()) {
                c = input_[pos_++];
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    require(pos_ < input_.size(), "unfinished quote escape");
                    c = input_[pos_++];
                }
                s.push_back(c);
            }
            require(closed, "unterminated quoted phrase");
            cur_ = {Lexeme::Phrase, std::move(s)};
            return;
        }
        size_t start = pos_ - 1;
        while (pos_ < input_.size() && !std::isspace(static_cast<unsigned char>(input_[pos_])) &&
               input_[pos_] != '(' && input_[pos_] != ')' && input_[pos_] != '"')
            ++pos_;
        cur_ = {Lexeme::Word, std::string(input_.substr(start, pos_ - start))};
    }
    /// @brief Проверяет точное совпадение текущей лексемы-слова с s, учитывая регистр.
    bool word(std::string_view s) const { return cur_.type == Lexeme::Word && cur_.value == s; }
    /// @brief Проверяет, обозначает ли текущая лексема оператор NEAR или ADJ с расстоянием.
    bool proximity() const {
        return cur_.type == Lexeme::Word &&
               (word("NEAR") || word("ADJ") || cur_.value.rfind("NEAR/", 0) == 0 ||
                cur_.value.rfind("ADJ/", 0) == 0);
    }
    /// @brief Создаёт узел AST и передаёт ему операнды; ограничивает общее число узлов.
    std::unique_ptr<Query> node(Op op, std::unique_ptr<Query> a = {},
                                std::unique_ptr<Query> b = {}) {
        require(++nodes_ <= 512, "query too complex (maximum 512 nodes)");
        auto q = std::make_unique<Query>();
        q->op = op;
        q->left = std::move(a);
        q->right = std::move(b);
        return q;
    }
    /// @brief Проверяет, что поддерево имеет координатную семантику для NEAR/ADJ.
    bool positional(const Query& q) const {
        return q.op == Op::Term || ((q.op == Op::Near || q.op == Op::Adj || q.op == Op::Or) &&
                                    positional(*q.left) && positional(*q.right));
    }
    /// @brief Разбирает терм, фразу либо выражение в скобках и нормализует текст.
    std::unique_ptr<Query> atom() {
        if (cur_.type == Lexeme::Left) {
            require(++depth_ <= 64, "parentheses nested too deeply");
            next();
            auto q = disjunction();
            require(cur_.type == Lexeme::Right, "expected closing parenthesis");
            next();
            --depth_;
            return q;
        }
        require(cur_.type == Lexeme::Word || cur_.type == Lexeme::Phrase,
                "expected term, phrase or parenthesis");
        require(!(word("AND") || word("OR") || word("NOT") || proximity()),
                "operator without left operand; unary NOT is not supported");
        auto ts = tokens(cur_.value);
        require(!ts.empty(), "term/phrase contains no searchable tokens");
        require(cur_.type == Lexeme::Phrase || ts.size() == 1,
                "use quotes for text containing several tokens");
        auto q = node(Op::Term);
        q->term = ts[0];
        // Точная фраза превращается в цепочку ADJ с расстоянием один.
        for (size_t i = 1; i < ts.size(); ++i) {
            auto t = node(Op::Term);
            t->term = ts[i];
            q = node(Op::Adj, std::move(q), std::move(t));
        }
        next();
        return q;
    }
    /// @brief Разбирает левоассоциативную цепочку NEAR/ADJ, проверяя расстояния и операнды.
    std::unique_ptr<Query> proximity_expr() {
        auto q = atom();
        while (proximity()) {
            std::string op = cur_.value;
            bool near = op.rfind("NEAR", 0) == 0;
            uint32_t distance = near ? 5 : 1;
            auto slash = op.find('/');
            if (slash != std::string::npos) {
                auto suffix = op.substr(slash + 1);
                require(!suffix.empty() && suffix.size() <= 10, "invalid proximity distance");
                uint64_t n = 0;
                for (char c : suffix) {
                    require(c >= '0' && c <= '9', "invalid proximity distance");
                    n = n * 10 + unsigned(c - '0');
                }
                require(n > 0 && n < END, "distance must be in [1, 4294967294]");
                distance = uint32_t(n);
            }
            next();
            auto r = atom();
            require(positional(*q) && positional(*r),
                    "NEAR/ADJ operands must be terms, phrases, proximity expressions or their OR "
                    "combinations");
            q = node(near ? Op::Near : Op::Adj, std::move(q), std::move(r));
            q->distance = distance;
        }
        return q;
    }
    /// @brief Разбирает цепочку AND/AND NOT с приоритетом выше OR.
    std::unique_ptr<Query> conjunction() {
        auto q = proximity_expr();
        while (word("AND")) {
            next();
            bool negative = word("NOT");
            if (negative)
                next();
            auto r = proximity_expr();
            q = node(negative ? Op::AndNot : Op::And, std::move(q), std::move(r));
        }
        return q;
    }
    /// @brief Разбирает цепочку OR, объединяя выражения более высокого приоритета.
    std::unique_ptr<Query> disjunction() {
        auto q = conjunction();
        while (word("OR")) {
            next();
            auto r = conjunction();
            q = node(Op::Or, std::move(q), std::move(r));
        }
        return q;
    }

  public:
    /// @brief Инициализирует разбор запроса и читает первую лексему; ограничивает длину входа.
    explicit Parser(std::string_view input) : input_(input) {
        require(input.size() <= 65536, "query is too long");
        next();
    }
    /// @brief Возвращает AST и требует, чтобы весь вход был разобран без лишних лексем.
    std::unique_ptr<Query> parse() {
        auto q = disjunction();
        require(cur_.type == Lexeme::End, "unexpected token; use explicit AND/OR between terms");
        return q;
    }
};
/// @brief Сортирует интервалы по границам и удаляет полностью совпадающие пары.
void deduplicate(std::vector<Span>& spans) {
    std::sort(spans.begin(), spans.end());
    spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
}
class Intersection final : public Iterator {
    std::vector<Iter> children_;
    uint32_t doc_ = END;
    std::vector<Span> none_;
    /// @brief Согласует дочерние потоки до общего docID >= target; при исчерпании возвращает END.
    uint32_t seek(uint32_t target) {
        uint32_t d = children_[0]->advance(target);
        for (size_t i = 1; d != END && i < children_.size();) {
            auto other = children_[i]->advance(d);
            if (other == d)
                ++i;
            else {
                // Один поток ушёл вперёд: подтягиваем ведущий и сверяем остальные заново.
                d = children_[0]->advance(other);
                i = 1;
            }
        }
        return doc_ = d;
    }

  public:
    /// @brief Выбирает наиболее редкий дочерний поток ведущим и ищет первое пересечение.
    explicit Intersection(std::vector<Iter> children) : children_(std::move(children)) {
        // Редкий поток обычно задаёт меньше кандидатов для остальных итераторов.
        std::sort(children_.begin(), children_.end(),
                  [](const Iter& a, const Iter& b) { return a->cost() < b->cost(); });
        seek(0);
    }
    /// @brief Возвращает текущий docID результата либо END.
    uint32_t doc() const override { return doc_; }
    /// @brief Продвигает результат к docID >= target; не возвращается назад после продвижения.
    uint32_t advance(uint32_t target) override {
        return doc_ == END || target <= doc_ ? doc_ : seek(target);
    }
    /// @brief Возвращает стоимость самого редкого дочернего потока как верхнюю оценку пересечения.
    uint64_t cost() const override { return children_[0]->cost(); }
    /// @brief Возвращает пустой список: булево пересечение не определяет позиционный интервал.
    const std::vector<Span>& spans() override { return none_; }
};
class Union final : public Iterator {
    Iter a_, b_;
    uint32_t doc_;
    std::vector<Span> spans_;
    bool cached_ = false;

  public:
    /// @brief Создаёт слияние двух потоков, начиная с меньшего текущего docID.
    Union(Iter a, Iter b)
        : a_(std::move(a)), b_(std::move(b)), doc_(std::min(a_->doc(), b_->doc())) {}
    /// @brief Возвращает текущий docID результата либо END.
    uint32_t doc() const override { return doc_; }
    /// @brief Продвигает результат к docID >= target; не возвращается назад после продвижения.
    uint32_t advance(uint32_t target) override {
        if (doc_ == END || target <= doc_)
            return doc_;
        // Позиционные интервалы предыдущего документа больше не актуальны.
        cached_ = false;
        return doc_ = std::min(a_->advance(target), b_->advance(target));
    }
    /// @brief Возвращает сумму стоимостей потоков как верхнюю оценку размера объединения.
    uint64_t cost() const override { return a_->cost() + b_->cost(); }
    /// @brief Объединяет и кеширует уникальные интервалы детей, совпавших с текущим docID.
    const std::vector<Span>& spans() override {
        if (!cached_) {
            spans_.clear();
            if (doc_ != END)
                for (auto* child : {a_.get(), b_.get()})
                    if (child->doc() == doc_) {
                        auto& s = child->spans();
                        spans_.insert(spans_.end(), s.begin(), s.end());
                    }
            deduplicate(spans_);
            cached_ = true;
        }
        return spans_;
    }
};
class Difference final : public Iterator {
    Iter a_, b_;
    uint32_t doc_;
    std::vector<Span> none_;
    /// @brief Пропускает документы левого потока, присутствующие в правом, начиная с target.
    uint32_t seek(uint32_t target) {
        auto d = a_->advance(target);
        // Правый поток никогда не обгоняется полным перебором: он ищет текущий docID слева.
        while (d != END && b_->advance(d) == d)
            d = a_->next();
        return doc_ = d;
    }

  public:
    /// @brief Создаёт разность потоков и находит первый документ, отсутствующий справа.
    Difference(Iter a, Iter b) : a_(std::move(a)), b_(std::move(b)) { seek(0); }
    /// @brief Возвращает текущий docID результата либо END.
    uint32_t doc() const override { return doc_; }
    /// @brief Продвигает результат к docID >= target; не возвращается назад после продвижения.
    uint32_t advance(uint32_t target) override {
        return doc_ == END || target <= doc_ ? doc_ : seek(target);
    }
    /// @brief Возвращает стоимость левого потока как верхнюю оценку размера разности.
    uint64_t cost() const override { return a_->cost(); }
    /// @brief Возвращает пустой список: AND NOT не определяет позиционный интервал.
    const std::vector<Span>& spans() override { return none_; }
};
// Перекрывающиеся интервалы не соединяются: одно вхождение терма не может
// одновременно удовлетворять обеим сторонам операции NEAR.
/// @brief Добавляет интервалы для упорядоченных неперекрывающихся пар из a и b.
/// Входы должны быть отсортированы по началу; расстояние между ближайшими краями — 1…distance.
/// Результаты дописываются в out без предварительной очистки и удаления дублей.
void ordered_join(const std::vector<Span>& a, const std::vector<Span>& b, uint32_t distance,
                  std::vector<Span>& out) {
    for (auto x : a) {
        // Верхняя граница исключает пересечения и повторное использование той же позиции.
        auto start = std::upper_bound(b.begin(), b.end(), x.end,
                                      [](uint32_t v, const Span& y) { return v < y.begin; });
        for (auto it = start; it != b.end() && uint64_t(it->begin) <= uint64_t(x.end) + distance;
             ++it)
            out.push_back({x.begin, it->end});
    }
}
class Proximity final : public Iterator {
    Iter a_, b_;
    uint32_t distance_, doc_ = END;
    bool ordered_;
    std::vector<Span> spans_;
    /// @brief Согласует docID и проверяет координаты до первого позиционного совпадения >= target.
    uint32_t seek(uint32_t target) {
        uint32_t d = a_->advance(target);
        spans_.clear();
        while (d != END) {
            auto other = b_->advance(d);
            if (other != d) {
                d = a_->advance(other);
                continue;
            }
            // Координаты запрашиваем только после совпадения docID обоих операндов.
            const auto& a = a_->spans();
            const auto& b = b_->spans();
            ordered_join(a, b, distance_, spans_);
            // NEAR допускает обратный порядок, ADJ — только порядок операндов.
            if (!ordered_)
                ordered_join(b, a, distance_, spans_);
            if (!spans_.empty()) {
                deduplicate(spans_);
                return doc_ = d;
            }
            d = a_->next();
        }
        return doc_ = END;
    }

  public:
    /// @brief Создаёт позиционный итератор: ordered=true для ADJ, false для NEAR.
    /// Сразу ищет первый документ с подходящей парой интервалов.
    Proximity(Iter a, Iter b, uint32_t distance, bool ordered)
        : a_(std::move(a)), b_(std::move(b)), distance_(distance), ordered_(ordered) {
        seek(0);
    }
    /// @brief Возвращает текущий docID результата либо END.
    uint32_t doc() const override { return doc_; }
    /// @brief Продвигает результат к docID >= target; не возвращается назад после продвижения.
    uint32_t advance(uint32_t target) override {
        return doc_ == END || target <= doc_ ? doc_ : seek(target);
    }
    /// @brief Возвращает меньшую стоимость операндов как верхнюю оценку числа совпадений.
    uint64_t cost() const override { return std::min(a_->cost(), b_->cost()); }
    /// @brief Возвращает найденные уникальные интервалы текущего позиционного совпадения.
    const std::vector<Span>& spans() override { return spans_; }
};
/// @brief Разворачивает вложенные AND в плоский список итераторов, сохраняя остальные операции.
void and_children(const Query& q, const Segment& s, SearchStats* stats, std::vector<Iter>& out) {
    if (q.op == Op::And) {
        and_children(*q.left, s, stats, out);
        and_children(*q.right, s, stats, out);
    } else
        out.push_back(compile_query(q, s, stats));
}
} // namespace
/// @brief Разбирает текст запроса в AST с проверкой синтаксиса и ограничений сложности.
std::unique_ptr<Query> parse_query(std::string_view text) {
    return Parser(text).parse();
}
/// @brief Преобразует AST в дерево ленивых итераторов одного сегмента.
/// Цепочки AND объединяются для выбора порядка пересечения по стоимости.
Iter compile_query(const Query& q, const Segment& s, SearchStats* stats) {
    if (q.op == Op::Term)
        return s.iterator(q.term, stats);
    if (q.op == Op::And) {
        std::vector<Iter> children;
        and_children(q, s, stats, children);
        return std::make_unique<Intersection>(std::move(children));
    }
    auto a = compile_query(*q.left, s, stats), b = compile_query(*q.right, s, stats);
    if (q.op == Op::Or)
        return std::make_unique<Union>(std::move(a), std::move(b));
    if (q.op == Op::AndNot)
        return std::make_unique<Difference>(std::move(a), std::move(b));
    return std::make_unique<Proximity>(std::move(a), std::move(b), q.distance, q.op == Op::Adj);
}
/// @brief Последовательно обходит сегменты, возвращая первые limit результатов и статистику.
/// count_all разрешает полный подсчёт; без него достижение лимита даёт неполный результат.
SearchResult search(const Index& index, const Query& query, size_t limit, bool count_all) {
    SearchResult result;
    if (limit == 0 && !count_all) {
        result.complete = false;
        return result;
    }
    // Диапазоны docID сегментов уже упорядочены: дополнительное слияние не требуется.
    for (auto& segment : index.segments) {
        auto it = compile_query(query, *segment, &result.stats);
        while (it->doc() != END) {
            ++result.matches;
            if (result.hits.size() < limit)
                result.hits.push_back(it->doc());
            if (!count_all && result.hits.size() >= limit) {
                // Без полного обхода это только нижняя граница числа совпадений.
                result.complete = false;
                return result;
            }
            it->next();
        }
    }
    return result;
}
} // namespace l0
