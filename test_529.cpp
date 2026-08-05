#include "expense_tags.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool ok, const std::string& label)
{
    if (ok)
    {
        std::cout << "✓ " << label << std::endl;
    }
    else
    {
        std::cerr << "✗ " << label << std::endl;
        ++failures;
    }
}

int main()
{
    std::cout << "=== Testing 529 Expense Tags ===" << std::endl;

    // --- computeTxnKey ---
    const std::string k1 = ExpenseTags::computeTxnKey("Capital One Balance", 1722400000, 120.00, "Target", 0);
    const std::string k2 = ExpenseTags::computeTxnKey("Capital One Balance", 1722400000, 120.00, "Target", 0);
    check(!k1.empty() && k1 == k2, "computeTxnKey is deterministic");
    check(k1.size() >= 3 && k1.substr(k1.size() - 2) == "-0", "computeTxnKey appends occurrence suffix");

    const std::string k_other_amount = ExpenseTags::computeTxnKey("Capital One Balance", 1722400000, 120.01, "Target", 0);
    const std::string k_other_notes = ExpenseTags::computeTxnKey("Capital One Balance", 1722400000, 120.00, "Walmart", 0);
    const std::string k_other_occ = ExpenseTags::computeTxnKey("Capital One Balance", 1722400000, 120.00, "Target", 1);
    check(k1 != k_other_amount, "amount changes the key");
    check(k1 != k_other_notes, "notes change the key");
    check(k1 != k_other_occ, "occurrence changes the key");

    // Sign must not matter: spend txs are stored negative but keyed on abs value.
    const std::string k_neg = ExpenseTags::computeTxnKey("Capital One Balance", 1722400000, -120.00, "Target", 0);
    check(k1 == k_neg, "key uses absolute amount");

    // --- KeyAssigner ---
    ExpenseTags::KeyAssigner assigner;
    const std::string a1 = assigner.next("Capital One Balance", 1722400000, 120.00, "Target");
    const std::string a2 = assigner.next("Capital One Balance", 1722400000, 120.00, "Target");
    const std::string a3 = assigner.next("Capital One Balance", 1722400000, 55.00, "Costco");
    check(a1 == k1, "first duplicate gets occurrence 0");
    check(a2 == k_other_occ, "second identical tuple gets occurrence 1");
    check(a3.substr(a3.size() - 2) == "-0", "different tuple restarts at occurrence 0");

    // --- sanitizeFilename ---
    check(ExpenseTags::sanitizeFilename("receipt.jpg") == "receipt.jpg", "clean name passes through");
    check(ExpenseTags::sanitizeFilename("../../etc/passwd") == "etc_passwd", "path traversal stripped");
    check(ExpenseTags::sanitizeFilename("a b/c.pdf") == "a_b_c.pdf", "slashes and spaces become underscores");
    check(ExpenseTags::sanitizeFilename("...") == "", "dot-only name rejected");
    check(ExpenseTags::sanitizeFilename("") == "", "empty name rejected");

    // --- extension allowlist / mime ---
    check(ExpenseTags::isAllowedReceiptExtension("r.JPG"), "jpg allowed case-insensitively");
    check(ExpenseTags::isAllowedReceiptExtension("r.pdf"), "pdf allowed");
    check(ExpenseTags::isAllowedReceiptExtension("r.heic"), "heic allowed");
    check(!ExpenseTags::isAllowedReceiptExtension("r.exe"), "exe rejected");
    check(!ExpenseTags::isAllowedReceiptExtension("noext"), "extensionless rejected");
    check(ExpenseTags::receiptMimeType("r.jpg") == "image/jpeg", "jpg mime");
    check(ExpenseTags::receiptMimeType("r.png") == "image/png", "png mime");
    check(ExpenseTags::receiptMimeType("r.pdf") == "application/pdf", "pdf mime");
    check(ExpenseTags::receiptMimeType("r.bin") == "application/octet-stream", "unknown mime falls back");

    // --- tags.json round-trip ---
    const std::string tags_path = "test_529_tags.json";
    std::remove(tags_path.c_str());

    std::vector<ExpenseTags::TagRecord> missing_result;
    check(ExpenseTags::loadTags(tags_path, missing_result) && missing_result.empty(),
          "loadTags on missing file returns empty success");

    ExpenseTags::TagRecord r1;
    r1.key = "abc123-0";
    r1.status = "qualified";
    r1.qualified_amount = 40.0;
    r1.receipts = {"target-receipt.jpg", "back-of-receipt.png"};
    r1.account = "Capital One Balance";
    r1.date = 1722400000;
    r1.amount = 120.0;
    r1.notes = "Target \"back to school\" run, aisle 5\\9";
    r1.category = "GENERAL_MERCHANDISE_SUPERSTORES";
    r1.created = 1722500000;

    ExpenseTags::TagRecord r2;
    r2.key = "def456-1";
    r2.status = "dismissed";
    r2.account = "Capital One Balance";
    r2.date = 1722300000;
    r2.amount = 15.5;
    r2.notes = "Chipotle";
    r2.category = "FOOD_AND_DRINK_RESTAURANT";
    r2.created = 1722500001;

    check(ExpenseTags::saveTags(tags_path, {r1, r2}), "saveTags succeeds");

    std::vector<ExpenseTags::TagRecord> loaded;
    check(ExpenseTags::loadTags(tags_path, loaded), "loadTags succeeds");
    check(loaded.size() == 2, "loadTags returns both records");
    if (loaded.size() == 2)
    {
        check(loaded[0].key == r1.key && loaded[0].status == "qualified", "record 1 identity round-trips");
        check(std::abs(loaded[0].qualified_amount - 40.0) < 1e-9, "qualified_amount round-trips");
        check(loaded[0].receipts.size() == 2 && loaded[0].receipts[1] == "back-of-receipt.png",
              "receipts array round-trips");
        check(loaded[0].notes == r1.notes, "quotes and backslashes in notes round-trip");
        check(loaded[0].date == r1.date && std::abs(loaded[0].amount - 120.0) < 1e-9,
              "denormalized fields round-trip");
        check(loaded[1].status == "dismissed" && loaded[1].receipts.empty(),
              "dismissed record round-trips with no receipts");
    }

    {
        std::ofstream corrupt(tags_path);
        corrupt << "{not json";
    }
    std::vector<ExpenseTags::TagRecord> corrupt_result;
    check(!ExpenseTags::loadTags(tags_path, corrupt_result), "loadTags fails on corrupt file");

    std::remove(tags_path.c_str());

    if (failures > 0)
    {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "All checks passed" << std::endl;
    return 0;
}
