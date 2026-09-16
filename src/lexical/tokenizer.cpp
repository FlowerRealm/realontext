#include "lexical/tokenizer.h"

#include <string>
#include <vector>

#include <sqlite3.h>

namespace lexical {

namespace {

// Longer runs are hashes, base64 and minified blobs, not words anyone searches for.
constexpr size_t max_word = 64;

bool word_byte(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}
bool upper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
bool lower_or_digit(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
char fold(unsigned char c) { return upper(c) ? static_cast<char>(c | 0x20) : static_cast<char>(c); }

// Sub-word boundaries: underscores, fooBar, HTTPServer.
std::vector<std::string> parts(std::string_view w)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (cur.size() >= 2)
            out.push_back(cur);
        cur.clear();
    };
    for (size_t k = 0; k < w.size(); k++) {
        unsigned char c = w[k];
        if (c == '_') {
            flush();
            continue;
        }
        if (k > 0 && upper(c) &&
            (lower_or_digit(w[k - 1]) || (upper(w[k - 1]) && k + 1 < w.size() && lower_or_digit(w[k + 1])) ))
            flush();
        cur.push_back(fold(c));
    }
    flush();
    return out;
}

bool word(std::string_view w, size_t start, bool document, const Emit& emit)
{
    if (w.size() > max_word)
        return true;
    std::string whole;
    for (unsigned char c : w)
        whole.push_back(fold(c));
    bool first = true;
    if (whole.size() >= 2) {
        if (!emit(whole, false, start, start + w.size()))
            return false;
        first = false;
    }
    if (!document)
        return true;
    std::vector<std::string> subs = parts(w);
    if (subs.size() < 2)
        return true;
    for (const std::string& p : subs) {
        if (!emit(p, !first, start, start + w.size()))
            return false;
        first = false;
    }
    return true;
}

// ---------------------------------------------------------------- FTS5 glue

int create(void*, const char**, int, Fts5Tokenizer** out)
{
    static int instance;
    *out = reinterpret_cast<Fts5Tokenizer*>(&instance);
    return SQLITE_OK;
}

void destroy(Fts5Tokenizer*) {}

int run(Fts5Tokenizer*, void* ctx, int flags, const char* text, int n,
        int (*token)(void*, int, const char*, int, int, int))
{
    int rc = SQLITE_OK;
    bool document = (flags & FTS5_TOKENIZE_QUERY) == 0;
    tokenize(std::string_view(text, n), document, [&](std::string_view t, bool colocated, size_t s, size_t e) {
        rc = token(ctx, colocated ? FTS5_TOKEN_COLOCATED : 0, t.data(), static_cast<int>(t.size()),
                   static_cast<int>(s), static_cast<int>(e));
        return rc == SQLITE_OK;
    });
    return rc;
}

} // namespace

void tokenize(std::string_view text, bool document, const Emit& emit)
{
    size_t i = 0;
    while (i < text.size()) {
        if (!word_byte(text[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < text.size() && word_byte(text[j]))
            j++;
        if (!word(text.substr(i, j - i), i, document, emit))
            return;
        i = j;
    }
}

Status register_tokenizer(sqlite3* db)
{
    fts5_api* api = nullptr;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT fts5(?1)", -1, &s, nullptr) != SQLITE_OK)
        return Err{std::string("fts5 unavailable: ") + sqlite3_errmsg(db)};
    sqlite3_bind_pointer(s, 1, &api, "fts5_api_ptr", nullptr);
    sqlite3_step(s);
    sqlite3_finalize(s);
    if (!api)
        return Err{"fts5 unavailable"};
    static fts5_tokenizer tokenizer{create, destroy, run};
    if (api->xCreateTokenizer(api, "code", nullptr, &tokenizer, nullptr) != SQLITE_OK)
        return Err{std::string("register tokenizer: ") + sqlite3_errmsg(db)};
    return ok;
}

} // namespace lexical
