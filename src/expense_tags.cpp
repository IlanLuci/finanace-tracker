#include "expense_tags.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace
{
    std::string keyBase(const std::string& account, time_t date, double amount,
                        const std::string& notes)
    {
        const long long cents = std::llround(std::abs(amount) * 100.0);
        std::ostringstream base;
        base << account << "|" << static_cast<long long>(date) << "|" << cents << "|" << notes;
        return base.str();
    }

    std::string lowerExtension(const std::string& filename)
    {
        const size_t dot = filename.find_last_of('.');
        if (dot == std::string::npos || dot + 1 >= filename.size())
        {
            return "";
        }
        std::string ext = filename.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    std::string jsonEscape(const std::string& value)
    {
        std::ostringstream out;
        for (char c : value)
        {
            switch (c)
            {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out << buf;
                }
                else
                {
                    out << c;
                }
            }
        }
        return out.str();
    }

    // Minimal scanner for the exact shape saveTags writes: an array of
    // flat objects with string / number / string-array values. Anything
    // else is a parse failure — this file is only ever written by us.
    struct TagScanner
    {
        const std::string& text;
        size_t pos = 0;

        explicit TagScanner(const std::string& t) : text(t) {}

        void skipWhitespace()
        {
            while (pos < text.size() &&
                   std::isspace(static_cast<unsigned char>(text[pos])))
            {
                ++pos;
            }
        }

        bool consume(char expected)
        {
            skipWhitespace();
            if (pos < text.size() && text[pos] == expected)
            {
                ++pos;
                return true;
            }
            return false;
        }

        bool peek(char expected)
        {
            skipWhitespace();
            return pos < text.size() && text[pos] == expected;
        }

        bool parseString(std::string& out)
        {
            if (!consume('"')) return false;
            out.clear();
            while (pos < text.size())
            {
                const char c = text[pos++];
                if (c == '"') return true;
                if (c == '\\')
                {
                    if (pos >= text.size()) return false;
                    const char esc = text[pos++];
                    switch (esc)
                    {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        if (pos + 4 > text.size()) return false;
                        const std::string hex = text.substr(pos, 4);
                        pos += 4;
                        const long code = std::strtol(hex.c_str(), nullptr, 16);
                        // We only ever emit \u00XX for control chars.
                        out += static_cast<char>(code & 0xFF);
                        break;
                    }
                    default: return false;
                    }
                }
                else
                {
                    out += c;
                }
            }
            return false;
        }

        bool parseNumber(double& out)
        {
            skipWhitespace();
            const size_t start = pos;
            while (pos < text.size() &&
                   (std::isdigit(static_cast<unsigned char>(text[pos])) ||
                    text[pos] == '-' || text[pos] == '+' ||
                    text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E'))
            {
                ++pos;
            }
            if (pos == start) return false;
            try
            {
                out = std::stod(text.substr(start, pos - start));
            }
            catch (...)
            {
                return false;
            }
            return true;
        }
    };
}

namespace ExpenseTags
{
    std::string computeTxnKey(const std::string& account, time_t date, double amount,
                              const std::string& notes, int occurrence)
    {
        // FNV-1a 64-bit over the composite tuple.
        unsigned long long hash = 14695981039346656037ULL;
        for (unsigned char c : keyBase(account, date, amount, notes))
        {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        std::ostringstream out;
        out << std::hex << hash << "-" << std::dec << occurrence;
        return out.str();
    }

    std::string KeyAssigner::next(const std::string& account, time_t date, double amount,
                                  const std::string& notes)
    {
        const std::string base = keyBase(account, date, amount, notes);
        const int occurrence = occurrence_counts[base]++;
        return computeTxnKey(account, date, amount, notes, occurrence);
    }

    bool saveTags(const std::string& file_path,
                  const std::vector<TagRecord>& tags)
    {
        std::ostringstream out;
        out << "[";
        for (size_t i = 0; i < tags.size(); ++i)
        {
            const TagRecord& tag = tags[i];
            if (i > 0) out << ",";
            out << "\n  {"
                << "\"key\":\"" << jsonEscape(tag.key) << "\","
                << "\"status\":\"" << jsonEscape(tag.status) << "\","
                << "\"qualified_amount\":" << tag.qualified_amount << ","
                << "\"receipts\":[";
            for (size_t r = 0; r < tag.receipts.size(); ++r)
            {
                if (r > 0) out << ",";
                out << "\"" << jsonEscape(tag.receipts[r]) << "\"";
            }
            out << "],"
                << "\"account\":\"" << jsonEscape(tag.account) << "\","
                << "\"date\":" << static_cast<long long>(tag.date) << ","
                << "\"amount\":" << tag.amount << ","
                << "\"notes\":\"" << jsonEscape(tag.notes) << "\","
                << "\"category\":\"" << jsonEscape(tag.category) << "\","
                << "\"created\":" << static_cast<long long>(tag.created)
                << "}";
        }
        out << "\n]\n";

        const std::string temp_path = file_path + ".tmp";
        {
            std::ofstream file(temp_path, std::ios::trunc);
            if (!file.is_open()) return false;
            file << out.str();
            if (!file.good()) return false;
        }
        return std::rename(temp_path.c_str(), file_path.c_str()) == 0;
    }

    bool loadTags(const std::string& file_path,
                  std::vector<TagRecord>& out)
    {
        out.clear();
        std::ifstream file(file_path);
        if (!file.is_open())
        {
            return true; // missing file == empty store
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const std::string text = buffer.str();

        TagScanner scanner(text);
        if (!scanner.consume('[')) return false;
        if (scanner.consume(']')) return true;

        while (true)
        {
            if (!scanner.consume('{')) return false;
            TagRecord tag;
            if (!scanner.peek('}'))
            {
                while (true)
                {
                    std::string field;
                    if (!scanner.parseString(field)) return false;
                    if (!scanner.consume(':')) return false;

                    if (field == "receipts")
                    {
                        if (!scanner.consume('[')) return false;
                        if (!scanner.consume(']'))
                        {
                            while (true)
                            {
                                std::string receipt;
                                if (!scanner.parseString(receipt)) return false;
                                tag.receipts.push_back(receipt);
                                if (scanner.consume(']')) break;
                                if (!scanner.consume(',')) return false;
                            }
                        }
                    }
                    else if (field == "qualified_amount" || field == "date" ||
                             field == "amount" || field == "created")
                    {
                        double value = 0.0;
                        if (!scanner.parseNumber(value)) return false;
                        if (field == "qualified_amount") tag.qualified_amount = value;
                        else if (field == "date") tag.date = static_cast<time_t>(value);
                        else if (field == "amount") tag.amount = value;
                        else tag.created = static_cast<time_t>(value);
                    }
                    else
                    {
                        std::string value;
                        if (!scanner.parseString(value)) return false;
                        if (field == "key") tag.key = value;
                        else if (field == "status") tag.status = value;
                        else if (field == "account") tag.account = value;
                        else if (field == "notes") tag.notes = value;
                        else if (field == "category") tag.category = value;
                        // unknown string fields are ignored for forward compat
                    }

                    if (scanner.consume('}')) break;
                    if (!scanner.consume(',')) return false;
                }
            }
            else
            {
                scanner.consume('}');
            }
            out.push_back(tag);
            if (scanner.consume(']')) break;
            if (!scanner.consume(',')) return false;
        }
        return true;
    }

    std::string sanitizeFilename(const std::string& raw)
    {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw)
        {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) || c == '.' || c == '-')
            {
                out += c;
            }
            else if (c == '/' || c == '\\' || c == ' ' || c == '_')
            {
                out += '_';
            }
            // every other character is dropped
        }
        // Collapse leading dots/underscores so ".." and dotfiles can't survive.
        size_t start = 0;
        while (start < out.size() && (out[start] == '.' || out[start] == '_'))
        {
            ++start;
        }
        out.erase(0, start);
        // Reject names that are empty or all dots after cleaning.
        if (out.empty() || out.find_first_not_of('.') == std::string::npos)
        {
            return "";
        }
        return out;
    }

    bool isAllowedReceiptExtension(const std::string& filename)
    {
        const std::string ext = lowerExtension(filename);
        return ext == "jpg" || ext == "jpeg" || ext == "png" ||
               ext == "heic" || ext == "webp" || ext == "pdf";
    }

    std::string receiptMimeType(const std::string& filename)
    {
        const std::string ext = lowerExtension(filename);
        if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
        if (ext == "png") return "image/png";
        if (ext == "heic") return "image/heic";
        if (ext == "webp") return "image/webp";
        if (ext == "pdf") return "application/pdf";
        return "application/octet-stream";
    }
}
