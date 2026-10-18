#pragma once
#include "index.hpp"
namespace l0 {
enum class Op { Term, And, Or, AndNot, Near, Adj };
struct Query {
    Op op = Op::Term;
    std::string term;
    uint32_t distance = 1;
    std::unique_ptr<Query> left, right;
};
/// @brief Строит AST запроса с приоритетами операторов и проверкой позиционных операндов.
/// Некорректный синтаксис или превышение пределов сложности приводят к исключению.
std::unique_ptr<Query> parse_query(std::string_view text);
/// @brief Создаёт дерево итераторов для запроса на одном сегменте.
/// Сегмент и необязательный stats должны существовать дольше возвращённого итератора.
Iter compile_query(const Query& query, const Segment& segment, SearchStats* stats = nullptr);
struct SearchResult {
    std::vector<uint32_t> hits;
    uint64_t matches = 0;
    bool complete = true;
    SearchStats stats;
};
/// @brief Выполняет запрос по сегментам и сохраняет не более limit первых docID.
/// При count_all=true подсчитывает все совпадения; иначе останавливается по лимиту.
/// complete=false означает, что matches является только нижней границей.
SearchResult search(const Index& index, const Query& query, size_t limit = 20,
                    bool count_all = false);
} // namespace l0
