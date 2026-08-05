#include "expense_tags.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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
}

namespace ExpenseTags
{
    std::string computeTxnKey(const std::string& account, time_t date, double amount,
                              const std::string& notes, int occurrence)
    {
        // FNV-1a 64-bit over the composite tuple.
        unsigned long long hash = 1469598103934665603ULL;
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

    bool loadTags(const std::string& file_path, std::vector<TagRecord>& out)
    {
        (void)file_path;
        (void)out;
        return false; // implemented in Task 2
    }

    bool saveTags(const std::string& file_path, const std::vector<TagRecord>& tags)
    {
        (void)file_path;
        (void)tags;
        return false; // implemented in Task 2
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
